/*
 * Intel High Definition Audio controller and codec driver.
 *
 * The controller talks to the codecs on its link through two rings (CORB:
 * commands out, RIRB: responses in) and moves samples with stream
 * descriptors: each one plays or records a cyclic DMA buffer described by a
 * buffer descriptor list and raises an interrupt after every entry.
 *
 * Codecs are trees of widgets. The driver walks them once and looks for
 *   - an output path: output pin (line out, speaker, headphones) ... DAC
 *   - an input path:  ADC ... input pin (microphone, line in)
 * selects the connections along the paths, opens the amplifiers at 0 dB
 * and binds the converters to its two streams. All volume control is left
 * to the audio server's mixer.
 *
 * The sound of a monitor on HDMI or DisplayPort is a codec, too: a digital
 * one, part of the graphics chip, with a converter and a pin for each
 * place the graphics side can send sound to. The driver takes the first
 * such pair: the stream goes to the converter, and the graphics driver
 * puts what arrives there into the picture's signal (for AMD graphics:
 * its audio endpoint 0, see drivers/graphics/amd_gpu.c). A controller with
 * such a codec becomes an audio device of its own, "Monitor sound".
 *
 * The format is fixed: 48 kHz, 16 bits, stereo. Each stream buffer has
 * FRAGMENTS periods of 10 ms; playback keeps the next period filled ahead
 * of the hardware position (latency 10-20 ms on top of the device ring).
 *
 * Tested with QEMU's codecs (hda-output, hda-micro, hda-duplex). Not yet:
 * jack detection, other sample formats, suspend; of the digital codecs
 * only AMD's (1002:aa01).
 */

#include "audio/device/audio_device.h"
#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"

#include "core/arch.h"
#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "scheduler/mutex.h"
#include "scheduler/thread.h"

/* Controller registers */
#define REG_GCAP      0x00
#define REG_GCTL      0x08
#define REG_STATESTS  0x0E
#define REG_INTCTL    0x20
#define REG_INTSTS    0x24
#define REG_CORBLBASE 0x40
#define REG_CORBUBASE 0x44
#define REG_CORBWP    0x48
#define REG_CORBRP    0x4A
#define REG_CORBCTL   0x4C
#define REG_CORBSIZE  0x4E
#define REG_RIRBLBASE 0x50
#define REG_RIRBUBASE 0x54
#define REG_RIRBWP    0x58
#define REG_RINTCNT   0x5A
#define REG_RIRBCTL   0x5C
#define REG_RIRBSTS   0x5D
#define REG_RIRBSIZE  0x5E
#define REG_STREAMS   0x80 /* + 0x20 * index; input streams first */

#define GCTL_RESET    (1u << 0) /* 0 = in reset */
#define INTCTL_GLOBAL (1u << 31)
#define RING_RUN      (1u << 1)
#define RIRB_INTERRUPT (1u << 0) /* RIRBCTL: flag responses in RIRBSTS */

/* Stream descriptor registers */
#define SD_CTL   0x00 /* 24 bits; the stream tag is in the third byte */
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0C
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

#define SD_CTL_RESET (1u << 0)
#define SD_CTL_RUN   (1u << 1)
#define SD_CTL_IOCE  (1u << 2) /* interrupt on completion */
#define SD_CTL2_TRAFFIC_PRIORITY 0x04 /* in the third byte: the stream's DMA is not snooped */
#define SD_STS_BCIS  (1u << 2) /* buffer completion */
#define SD_STS_ALL   0x1C

/* Verbs */
#define VERB_GET_PARAMETER   0xF00
#define VERB_GET_CONNECTION  0xF02
#define VERB_GET_CONFIG      0xF1C
#define VERB_SET_SELECT      0x701
#define VERB_SET_POWER       0x705
#define VERB_SET_STREAM      0x706
#define VERB_SET_PIN_CONTROL 0x707
#define VERB_SET_EAPD        0x70C
#define VERB_SET_DIGITAL     0x70D /* a digital converter: bit 0 on */
#define VERB_SET_CHANNELS    0x72D /* a converter: channels - 1 */
/* The codec of AMD graphics (vendor 1002): verbs of its own */
#define ATI_SET_RAMP_RATE    0x770
#define ATI_SET_ALLOCATION   0x771 /* which loudspeakers the channels are (0: front left and right) */
#define ATI_SET_DOWNMIX      0x772
#define ATI_SET_SLOT_PAIR    0x777 /* slots 0+1 of the signal (0x778: 2+3, ...): stream channel << 4, bit 0 on */
#define ATI_SET_SLOT_ODD     0x785 /* slot 1 (0x786: 3, ...) when every channel is mapped by itself */
#define ATI_SLOT_ON          0x01
#define ATI_SET_CHANNEL_MODE 0x789 /* 1: every channel is mapped by itself */
#define VERB_GET_PIN_SENSE   0xF09 /* bit 31: something is plugged in; digital pins, bit 30: its description is valid */
#define VERB_GET_ELD_SIZE    0xF2E /* with payload 8: bytes of the monitor's description (ELD) the pin holds */
#define VERB_GET_ELD_BYTE    0xF2F /* payload: which byte; bit 31 of the answer: valid */
#define VERB4_SET_FORMAT     0x2 /* 4-bit verbs carry 16 bits */
#define VERB4_SET_AMP        0x3

#define PARAM_NODE_COUNT      0x04
#define PARAM_FUNCTION_TYPE   0x05
#define PARAM_WIDGET_CAPS     0x09
#define PARAM_PCM             0x0A /* sample rates (bits 11:0) and sizes (20:16) a converter takes */
#define PARAM_STREAM_FORMATS  0x0B /* bit 0 PCM, bit 2 AC-3 */
#define PARAM_PIN_CAPS        0x0C
#define PARAM_IN_AMP_CAPS     0x0D
#define PARAM_CONNECTION_LIST 0x0E
#define PARAM_OUT_AMP_CAPS    0x12

#define WIDGET_OUTPUT   0 /* DAC */
#define WIDGET_INPUT    1 /* ADC */
#define WIDGET_MIXER    2
#define WIDGET_SELECTOR 3
#define WIDGET_PIN      4

#define WCAP_TYPE(c)     (((c) >> 20) & 0xF)
#define WCAP_IN_AMP      (1u << 1)
#define WCAP_OUT_AMP     (1u << 2)
#define WCAP_AMP_OWN     (1u << 3) /* own amplifier parameters instead of the function group's */
#define WCAP_DIGITAL     (1u << 9)
#define WCAP_POWER       (1u << 10)
#define PINCAP_OUTPUT    (1u << 4)
#define PINCAP_INPUT     (1u << 5)
#define PINCAP_EAPD      (1u << 16)

#define AMP_OUTPUT (1u << 15)
#define AMP_INPUT  (1u << 14)
#define AMP_LEFT   (1u << 13)
#define AMP_RIGHT  (1u << 12)

/* 48 kHz base rate, no multiplier or divisor, 16 bits, 2 channels */
#define FORMAT_48K_16_STEREO 0x0011

#define RATE          48000
#define CHANNELS      2
#define PERIOD_FRAMES 480 /* 10 ms */
#define FRAGMENTS     8
#define FRAGMENT_BYTES (PERIOD_FRAMES * CHANNELS * sizeof(int16_t))
#define BUFFER_BYTES  (FRAGMENTS * FRAGMENT_BYTES)

#define TAG_PLAYBACK 1
#define TAG_CAPTURE  2

#define MAX_WIDGETS     96
#define MAX_CONNECTIONS 16
#define MAX_DEPTH       5
#define MAX_PATHS       4

typedef struct {
    uint8_t  nid;
    uint8_t  connection_count;
    uint8_t  connections[MAX_CONNECTIONS];
    uint32_t caps, pin_caps, config;
} hda_widget_t;

typedef struct {
    uint8_t      address;
    uint8_t      function_group;
    hda_widget_t widgets[MAX_WIDGETS];
    uint32_t     widget_count;
} hda_codec_t;

typedef struct {
    uint64_t address;
    uint32_t length;
    uint32_t flags; /* bit 0: interrupt on completion */
} hda_bdl_entry_t;

typedef struct {
    volatile uint8_t *regs;
    uint32_t          index;    /* bit in INTCTL / INTSTS */
    uint8_t           tag;
    dma_buffer_t      buffer, bdl;
    bool              running;
    uint32_t          last_lpib;
    uint64_t          position; /* bytes the hardware has gone through */
    uint64_t          handled;  /* fragments filled (playback) or delivered (capture) */
} hda_stream_t;

typedef struct {
    device_t         *device;
    volatile uint8_t *regs;
    uint32_t          irq;
    uint64_t          dma_limit;
    mutex_t           lock;     /* verbs */
    dma_buffer_t      rings;    /* CORB at 0, RIRB at half a page */
    uint32_t          corb_size, rirb_size, rirb_read;
    hda_stream_t      out, in;
    audio_device_t    audio;
    bool              monitor;  /* the output is a digital codec: the sound of a monitor */
    struct {
        uint8_t  address, pin, converter; /* of the codec, and the pair of widgets in use */
        uint32_t revision;
        bool     amd, pin_amp, pin_select, power;
    } sink;
    bool              no_snoop; /* the controller reads its buffers past the CPU's caches (AMD's HDMI controllers) */
} hda_t;

/* --- Registers ------------------------------------------------------------------- */

static uint8_t r8(volatile uint8_t *base, uint32_t offset)
{
    return base[offset];
}

static uint16_t r16(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint16_t *)(base + offset);
}

static uint32_t r32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void w8(volatile uint8_t *base, uint32_t offset, uint8_t value)
{
    base[offset] = value;
}

static void w16(volatile uint8_t *base, uint32_t offset, uint16_t value)
{
    *(volatile uint16_t *)(base + offset) = value;
}

static void w32(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

/* Poll until (register & mask) == value. */
static bool wait32(volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms)
{
    for (uint32_t waited = 0;; waited++) {
        if ((r32(base, offset) & mask) == value)
            return true;
        if (waited >= timeout_ms)
            return false;
        thread_sleep(1000000);
    }
}

/* --- Codec commands -------------------------------------------------------------- */

/* Send a verb through the CORB and wait for the response in the RIRB. */
static status_t verb_raw(hda_t *hda, uint32_t verb, uint32_t *response)
{
    uint32_t *corb = hda->rings.virt;
    uint64_t *rirb = (uint64_t *)((uint8_t *)hda->rings.virt + PAGE_SIZE / 2);

    mutex_lock(&hda->lock);
    uint32_t write = (r16(hda->regs, REG_CORBWP) + 1) % hda->corb_size;
    corb[write] = verb;
    w16(hda->regs, REG_CORBWP, (uint16_t)write);

    status_t status = STATUS_TIMEOUT;
    for (uint32_t tries = 0; tries < 2100; tries++) {
        if ((r16(hda->regs, REG_RIRBWP) & 0xFF) != hda->rirb_read) {
            hda->rirb_read = (hda->rirb_read + 1) % hda->rirb_size;
            uint64_t entry = rirb[hda->rirb_read];
            /* Acknowledge the response; controllers (QEMU's too) hold back the next one until then. */
            w8(hda->regs, REG_RIRBSTS, 0x05);
            if ((entry >> 32) & (1u << 4))
                continue; /* unsolicited */
            if (response)
                *response = (uint32_t)entry;
            status = STATUS_SUCCESS;
            break;
        }
        if (tries >= 2000)
            thread_sleep(1000000); /* codecs answer within microseconds; be patient with a slow one */
    }
    mutex_unlock(&hda->lock);
    return status;
}

/* A 12-bit verb with 8 bits of payload; 0 on failure. */
static uint32_t verb(hda_t *hda, const hda_codec_t *codec, uint8_t nid, uint32_t id, uint32_t payload)
{
    uint32_t response = 0;
    verb_raw(hda, (uint32_t)codec->address << 28 | (uint32_t)nid << 20 | id << 8 | (payload & 0xFF), &response);
    return response;
}

/* A 4-bit verb with 16 bits of payload. */
static void verb4(hda_t *hda, const hda_codec_t *codec, uint8_t nid, uint32_t id, uint32_t payload)
{
    verb_raw(hda, (uint32_t)codec->address << 28 | (uint32_t)nid << 20 | id << 16 | (payload & 0xFFFF), NULL);
}

static uint32_t parameter(hda_t *hda, const hda_codec_t *codec, uint8_t nid, uint32_t id)
{
    return verb(hda, codec, nid, VERB_GET_PARAMETER, id);
}

/* --- Codec topology -------------------------------------------------------------- */

static hda_widget_t *widget(hda_codec_t *codec, uint8_t nid)
{
    for (uint32_t i = 0; i < codec->widget_count; i++) {
        if (codec->widgets[i].nid == nid)
            return &codec->widgets[i];
    }
    return NULL;
}

static void read_connections(hda_t *hda, hda_codec_t *codec, hda_widget_t *w)
{
    uint32_t info = parameter(hda, codec, w->nid, PARAM_CONNECTION_LIST);
    bool wide = info & 0x80;
    uint32_t total = info & 0x7F, per_response = wide ? 2 : 4, bits = wide ? 16 : 8;
    uint32_t range_bit = 1u << (bits - 1), previous = 0;

    for (uint32_t i = 0; i < total; i += per_response) {
        uint32_t response = verb(hda, codec, w->nid, VERB_GET_CONNECTION, i);
        for (uint32_t k = 0; k < per_response && i + k < total; k++) {
            uint32_t entry = (response >> (k * bits)) & ((1u << bits) - 1);
            uint32_t nid = entry & (range_bit - 1);
            /* The range flag means "everything from the previous entry up to this one". */
            for (uint32_t n = (entry & range_bit) && previous ? previous + 1 : nid; n <= nid; n++) {
                if (w->connection_count < MAX_CONNECTIONS)
                    w->connections[w->connection_count++] = (uint8_t)n;
            }
            previous = nid;
        }
    }
}

static bool read_codec(hda_t *hda, hda_codec_t *codec)
{
    uint32_t groups = parameter(hda, codec, 0, PARAM_NODE_COUNT);

    for (uint32_t g = 0; g < (groups & 0xFF) && !codec->function_group; g++) {
        uint8_t nid = (uint8_t)(((groups >> 16) & 0xFF) + g);
        if ((parameter(hda, codec, nid, PARAM_FUNCTION_TYPE) & 0xFF) == 1) /* audio function group */
            codec->function_group = nid;
    }
    if (!codec->function_group)
        return false;
    verb(hda, codec, codec->function_group, VERB_SET_POWER, 0); /* D0 */
    thread_sleep(10000000);

    uint32_t nodes = parameter(hda, codec, codec->function_group, PARAM_NODE_COUNT);
    for (uint32_t i = 0; i < (nodes & 0xFF) && codec->widget_count < MAX_WIDGETS; i++) {
        hda_widget_t *w = &codec->widgets[codec->widget_count++];
        w->nid = (uint8_t)(((nodes >> 16) & 0xFF) + i);
        w->caps = parameter(hda, codec, w->nid, PARAM_WIDGET_CAPS);
        if (WCAP_TYPE(w->caps) == WIDGET_PIN) {
            w->pin_caps = parameter(hda, codec, w->nid, PARAM_PIN_CAPS);
            w->config = verb(hda, codec, w->nid, VERB_GET_CONFIG, 0);
        }
        if (w->caps & (1u << 8)) /* has a connection list */
            read_connections(hda, codec, w);
    }
    return codec->widget_count > 0;
}

/*
 * What a codec is made of, in the log: for codecs the driver finds no way through, which today are the digital
 * ones (the sound of an HDMI or DisplayPort monitor). Their pins can also say whether a monitor is there and
 * hold its description of what sound it takes (ELD), if the graphics side has passed it on.
 */
static void report_codec(hda_t *hda, hda_codec_t *codec)
{
    static const char *const types[16] = { "output", "input", "mixer", "selector", "pin", "power", "volume knob", "beep",
                                           "?", "?", "?", "?", "?", "?", "?", "vendor" };

    klog_debug("hda: %s: codec %u (vendor %08x, revision %08x), function group %u:", hda->device->name, codec->address,
               parameter(hda, codec, 0, 0), parameter(hda, codec, 0, 2), codec->function_group);
    for (uint32_t i = 0; i < codec->widget_count; i++) {
        const hda_widget_t *w = &codec->widgets[i];
        char from[3 * MAX_CONNECTIONS + 4] = "";
        size_t used = 0;

        for (uint32_t k = 0; k < w->connection_count; k++)
            used += (size_t)format(from + used, sizeof(from) - used, "%s%u", k ? "," : "", w->connections[k]);
        klog_debug("hda:   widget %u: %s%s, caps 0x%x%s%s", w->nid, types[WCAP_TYPE(w->caps)],
                   (w->caps & WCAP_DIGITAL) ? " (digital)" : "", w->caps, used ? ", from " : "", from);
        if (WCAP_TYPE(w->caps) == WIDGET_OUTPUT || WCAP_TYPE(w->caps) == WIDGET_INPUT)
            klog_debug("hda:     rates and sizes 0x%x, formats 0x%x, channels %u", parameter(hda, codec, w->nid, PARAM_PCM),
                       parameter(hda, codec, w->nid, PARAM_STREAM_FORMATS),
                       ((((w->caps >> 13) & 7) << 1) | (w->caps & 1)) + 1);
        if (WCAP_TYPE(w->caps) != WIDGET_PIN)
            continue;
        uint32_t sense = verb(hda, codec, w->nid, VERB_GET_PIN_SENSE, 0);
        uint32_t size = (w->caps & WCAP_DIGITAL) ? verb(hda, codec, w->nid, VERB_GET_ELD_SIZE, 0x08) & 0xFF : 0;
        klog_debug("hda:     pin caps 0x%x%s%s, default 0x%x, sense 0x%x (%s%s), description of %u bytes", w->pin_caps,
                   (w->pin_caps & (1u << 7)) ? " HDMI" : "", (w->pin_caps & (1u << 24)) ? " DisplayPort" : "", w->config,
                   sense, (sense & (1u << 31)) ? "something plugged in" : "nothing plugged in",
                   (sense & (1u << 30)) ? ", description valid" : "", size);
        if (size) {
            uint8_t eld[16];
            for (uint32_t k = 0; k < sizeof(eld); k++)
                eld[k] = (uint8_t)verb(hda, codec, w->nid, VERB_GET_ELD_BYTE, k);
            klog_debug("hda:     it starts %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                       eld[0], eld[1], eld[2], eld[3], eld[4], eld[5], eld[6], eld[7], eld[8], eld[9], eld[10], eld[11],
                       eld[12], eld[13], eld[14], eld[15]);
        }
    }
}

/*
 * Depth-first search along the connection lists from `nid` to a widget of
 * type `target` (for pins: one that passes pin_ok). path[] receives the
 * widgets from the start to the target.
 */
static uint32_t find_path(hda_codec_t *codec, uint8_t nid, uint32_t target, bool (*pin_ok)(const hda_widget_t *),
                          uint8_t *path, uint32_t depth)
{
    hda_widget_t *w = widget(codec, nid);

    if (!w || depth == MAX_DEPTH || (w->caps & WCAP_DIGITAL))
        return 0;
    path[depth] = nid;
    if (depth > 0 && WCAP_TYPE(w->caps) == target && (target != WIDGET_PIN || pin_ok(w)))
        return depth + 1;
    if (depth > 0 && WCAP_TYPE(w->caps) != WIDGET_MIXER && WCAP_TYPE(w->caps) != WIDGET_SELECTOR)
        return 0; /* paths only run through mixers and selectors */
    for (uint32_t i = 0; i < w->connection_count; i++) {
        uint32_t length = find_path(codec, w->connections[i], target, pin_ok, path, depth + 1);
        if (length)
            return length;
    }
    return 0;
}

static uint32_t default_device(const hda_widget_t *pin)
{
    return (pin->config >> 20) & 0xF;
}

static bool connected(const hda_widget_t *pin)
{
    return (pin->config >> 30) != 1; /* 1: no physical connection */
}

static bool output_pin(const hda_widget_t *pin)
{
    /* line out, speaker, headphones */
    return (pin->pin_caps & PINCAP_OUTPUT) && connected(pin) && default_device(pin) <= 2;
}

static bool input_pin(const hda_widget_t *pin)
{
    /* line in, microphone */
    return (pin->pin_caps & PINCAP_INPUT) && connected(pin) && (default_device(pin) == 0x8 || default_device(pin) == 0xA);
}

/* Unmute an amplifier at 0 dB (the "offset" step of its capabilities). */
static void open_amp(hda_t *hda, hda_codec_t *codec, const hda_widget_t *w, bool output, uint32_t index)
{
    uint8_t source = (w->caps & WCAP_AMP_OWN) ? w->nid : codec->function_group;
    uint32_t caps = parameter(hda, codec, source, output ? PARAM_OUT_AMP_CAPS : PARAM_IN_AMP_CAPS);
    verb4(hda, codec, w->nid, VERB4_SET_AMP,
          (output ? AMP_OUTPUT : AMP_INPUT) | AMP_LEFT | AMP_RIGHT | (index & 0xF) << 8 | (caps & 0x7F));
}

/* Make the path carry sound: path[0] is where the signal ends, path[length - 1] where it comes from. */
static void open_path(hda_t *hda, hda_codec_t *codec, const uint8_t *path, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        hda_widget_t *w = widget(codec, path[i]);
        uint32_t selected = 0;

        if (w->caps & WCAP_POWER)
            verb(hda, codec, w->nid, VERB_SET_POWER, 0);
        if (i + 1 < length) {
            for (uint32_t k = 0; k < w->connection_count; k++) {
                if (w->connections[k] == path[i + 1])
                    selected = k;
            }
            if (w->connection_count > 1 && WCAP_TYPE(w->caps) != WIDGET_MIXER)
                verb(hda, codec, w->nid, VERB_SET_SELECT, selected);
        }
        if ((w->caps & WCAP_IN_AMP) && i + 1 < length)
            open_amp(hda, codec, w, false, WCAP_TYPE(w->caps) == WIDGET_MIXER ? selected : 0);
        if (w->caps & WCAP_OUT_AMP)
            open_amp(hda, codec, w, true, 0);
    }
}

/*
 * Find and open the paths of one codec; returns the JELLY_AUDIO_* directions it serves.
 * Each stream goes to one codec only: all outputs of the first codec that has any, and one input.
 */
static uint32_t setup_codec(hda_t *hda, hda_codec_t *codec, bool want_playback, bool want_capture)
{
    uint8_t path[MAX_DEPTH];
    uint32_t flags = 0, outputs = 0;

    for (uint32_t i = 0; want_playback && i < codec->widget_count && outputs < MAX_PATHS; i++) {
        hda_widget_t *pin = &codec->widgets[i];
        if (WCAP_TYPE(pin->caps) != WIDGET_PIN || !output_pin(pin))
            continue;
        uint32_t length = find_path(codec, pin->nid, WIDGET_OUTPUT, NULL, path, 0);
        if (!length)
            continue;
        open_path(hda, codec, path, length);
        verb(hda, codec, pin->nid, VERB_SET_PIN_CONTROL, 0x40 | (default_device(pin) == 2 ? 0x80 : 0)); /* out, headphone amp */
        if (pin->pin_caps & PINCAP_EAPD)
            verb(hda, codec, pin->nid, VERB_SET_EAPD, 0x02);
        uint8_t dac = path[length - 1];
        verb(hda, codec, dac, VERB_SET_STREAM, TAG_PLAYBACK << 4);
        verb4(hda, codec, dac, VERB4_SET_FORMAT, FORMAT_48K_16_STEREO);
        klog_debug("hda: codec %u: output pin %u <- DAC %u", codec->address, pin->nid, dac);
        flags |= JELLY_AUDIO_PLAYBACK;
        outputs++;
    }

    for (uint32_t i = 0; want_capture && i < codec->widget_count; i++) {
        hda_widget_t *adc = &codec->widgets[i];
        if (WCAP_TYPE(adc->caps) != WIDGET_INPUT)
            continue;
        uint32_t length = find_path(codec, adc->nid, WIDGET_PIN, input_pin, path, 0);
        if (!length)
            continue;
        hda_widget_t *pin = widget(codec, path[length - 1]);
        open_path(hda, codec, path, length);
        verb(hda, codec, pin->nid, VERB_SET_PIN_CONTROL, 0x20 | (default_device(pin) == 0xA ? 0x04 : 0)); /* in, mic bias */
        verb(hda, codec, adc->nid, VERB_SET_STREAM, TAG_CAPTURE << 4);
        verb4(hda, codec, adc->nid, VERB4_SET_FORMAT, FORMAT_48K_16_STEREO);
        klog_debug("hda: codec %u: ADC %u <- input pin %u", codec->address, adc->nid, pin->nid);
        flags |= JELLY_AUDIO_CAPTURE;
        break;
    }
    return flags;
}

/*
 * Tell the monitor's codec (again) what it is to do: the pin on, the converter fed by the playback stream
 * with two channels of 16 bits at 48 kHz. Thread context.
 */
static void monitor_program(hda_t *hda)
{
    hda_codec_t codec = { .address = hda->sink.address }; /* (for verb(): only the address is looked at) */
    uint8_t pin = hda->sink.pin, converter = hda->sink.converter;

    if (hda->sink.power) {
        verb(hda, &codec, converter, VERB_SET_POWER, 0);
        verb(hda, &codec, pin, VERB_SET_POWER, 0);
    }
    if (hda->sink.amd) {
        /* As Linux's patch_hdmi.c: nothing mixed down, channels mapped one by one, the default ramp. */
        bool single = (hda->sink.revision & 0xFF00) >= 0x0300;

        verb(hda, &codec, pin, ATI_SET_DOWNMIX, 0);
        if (single) {
            verb(hda, &codec, pin, ATI_SET_CHANNEL_MODE, 1);
            verb(hda, &codec, converter, ATI_SET_RAMP_RATE, 180);
        }
        verb(hda, &codec, pin, ATI_SET_ALLOCATION, 0);
        /*
         * The eight channel slots of the signal: each one is off until it is given a channel of the stream.
         * Front left and right are slots 0 and 1; the other six stay silent. (Older codecs map pairs.)
         */
        for (uint32_t slot = 0; slot < 8; slot++) {
            uint32_t setup = slot < CHANNELS ? slot << 4 | ATI_SLOT_ON : 0;
            if (!(slot & 1))
                verb(hda, &codec, pin, ATI_SET_SLOT_PAIR + slot / 2, setup);
            else if (single)
                verb(hda, &codec, pin, ATI_SET_SLOT_ODD + slot / 2, setup);
        }
    }
    if (hda->sink.pin_select)
        verb(hda, &codec, pin, VERB_SET_SELECT, 0);
    if (hda->sink.pin_amp)
        verb4(hda, &codec, pin, VERB4_SET_AMP, AMP_OUTPUT | AMP_LEFT | AMP_RIGHT);
    verb(hda, &codec, pin, VERB_SET_PIN_CONTROL, 0x40); /* out */
    verb(hda, &codec, converter, VERB_SET_CHANNELS, CHANNELS - 1);
    verb(hda, &codec, converter, VERB_SET_DIGITAL, 0x01);
    verb(hda, &codec, converter, VERB_SET_STREAM, TAG_PLAYBACK << 4);
    verb4(hda, &codec, converter, VERB4_SET_FORMAT, FORMAT_48K_16_STEREO);
}

/*
 * The sound of a monitor: a digital codec of a graphics chip. Its first output pin that is wired to a
 * connector, with the converter that feeds it, gets the playback stream: two channels of 16 bits at 48 kHz,
 * which every such codec and every monitor with loudspeakers takes. Only for codecs whose graphics side this
 * system drives; the others would be outputs that never sound.
 */
static uint32_t setup_monitor_codec(hda_t *hda, hda_codec_t *codec)
{
    uint32_t id = parameter(hda, codec, 0, 0);
    bool amd = (id >> 16) == 0x1002;

    if (!amd)
        return 0;
    for (uint32_t i = 0; i < codec->widget_count; i++) {
        hda_widget_t *pin = &codec->widgets[i];
        if (WCAP_TYPE(pin->caps) != WIDGET_PIN || !(pin->caps & WCAP_DIGITAL) || !(pin->pin_caps & PINCAP_OUTPUT) ||
            !connected(pin) || !pin->connection_count)
            continue;
        hda_widget_t *converter = widget(codec, pin->connections[0]);
        if (!converter || WCAP_TYPE(converter->caps) != WIDGET_OUTPUT)
            continue;
        hda->sink.address = codec->address;
        hda->sink.pin = pin->nid;
        hda->sink.converter = converter->nid;
        hda->sink.revision = parameter(hda, codec, 0, 2);
        hda->sink.amd = amd;
        hda->sink.pin_amp = pin->caps & WCAP_OUT_AMP;
        hda->sink.pin_select = pin->connection_count > 1;
        hda->sink.power = (pin->caps | converter->caps) & WCAP_POWER;
        hda->monitor = true;
        monitor_program(hda);
        klog_info("hda: codec %u: the sound of a monitor: pin %u <- converter %u", codec->address, pin->nid,
                  converter->nid);
        return JELLY_AUDIO_PLAYBACK;
    }
    return 0;
}

/* --- Streams --------------------------------------------------------------------- */

static void to_memory(const void *start, size_t bytes);

static status_t stream_alloc(hda_t *hda, hda_stream_t *stream, uint32_t index, uint8_t tag)
{
    stream->regs = hda->regs + REG_STREAMS + 0x20 * index;
    stream->index = index;
    stream->tag = tag;
    status_t status = dma_alloc(hda->device, BUFFER_BYTES, hda->dma_limit, &stream->buffer);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(hda->device, FRAGMENTS * sizeof(hda_bdl_entry_t), hda->dma_limit, &stream->bdl);
    if (STATUS_IS_ERROR(status))
        return status;
    hda_bdl_entry_t *bdl = stream->bdl.virt;
    for (uint32_t i = 0; i < FRAGMENTS; i++)
        bdl[i] = (hda_bdl_entry_t){ stream->buffer.phys + i * FRAGMENT_BYTES, FRAGMENT_BYTES, 1 };
    to_memory(bdl, FRAGMENTS * sizeof(hda_bdl_entry_t));
    return STATUS_SUCCESS;
}

/*
 * What the controller is about to read out of memory: out of the CPU's caches first. A controller may read
 * its buffers without the caches being asked ("no snoop", AMD's HDMI controllers), and would then play what
 * was in memory before: silence. (Linux maps the buffers write-combining for these.)
 */
static void to_memory(const void *start, size_t bytes)
{
    const uint8_t *p = start;

    for (size_t offset = 0; offset < bytes; offset += 64)
        __asm__ volatile("clflush (%0)" : : "r"(p + offset) : "memory");
    __asm__ volatile("mfence" : : : "memory");
}

static int16_t *fragment(hda_stream_t *stream, uint64_t number)
{
    return (int16_t *)((uint8_t *)stream->buffer.virt + (number % FRAGMENTS) * FRAGMENT_BYTES);
}

static void stream_stop(hda_t *hda, hda_stream_t *stream)
{
    uint64_t flags = arch_interrupts_save();
    stream->running = false;
    w32(hda->regs, REG_INTCTL, r32(hda->regs, REG_INTCTL) & ~(1u << stream->index));
    w8(stream->regs, SD_CTL, r8(stream->regs, SD_CTL) & ~(SD_CTL_RUN | SD_CTL_IOCE));
    arch_interrupts_restore(flags);
    for (int i = 0; i < 20 && (r8(stream->regs, SD_CTL) & SD_CTL_RUN); i++)
        thread_sleep(1000000);
    w8(stream->regs, SD_STS, SD_STS_ALL);
}

static status_t stream_start(hda_t *hda, hda_stream_t *stream, bool playback)
{
    /* Reset the stream: into reset, out of reset. */
    w8(stream->regs, SD_CTL, SD_CTL_RESET);
    for (int i = 0; i < 20 && !(r8(stream->regs, SD_CTL) & SD_CTL_RESET); i++)
        thread_sleep(1000000);
    w8(stream->regs, SD_CTL, 0);
    for (int i = 0; i < 20 && (r8(stream->regs, SD_CTL) & SD_CTL_RESET); i++)
        thread_sleep(1000000);
    if (r8(stream->regs, SD_CTL) & SD_CTL_RESET)
        return STATUS_DEVICE_ERROR;

    memset(stream->buffer.virt, 0, BUFFER_BYTES);
    stream->position = 0;
    stream->last_lpib = 0;
    stream->handled = 0;
    if (playback) {
        /* Two periods ahead of the hardware from the start */
        for (; stream->handled < 2; stream->handled++)
            audio_playback_pull(&hda->audio, fragment(stream, stream->handled), PERIOD_FRAMES);
    }
    to_memory(stream->buffer.virt, BUFFER_BYTES);

    w32(stream->regs, SD_BDPL, (uint32_t)stream->bdl.phys);
    w32(stream->regs, SD_BDPU, (uint32_t)(stream->bdl.phys >> 32));
    w32(stream->regs, SD_CBL, BUFFER_BYTES);
    w16(stream->regs, SD_LVI, FRAGMENTS - 1);
    w16(stream->regs, SD_FMT, FORMAT_48K_16_STEREO);
    w8(stream->regs, SD_CTL + 2, (uint8_t)(stream->tag << 4 | (hda->no_snoop ? SD_CTL2_TRAFFIC_PRIORITY : 0)));
    w8(stream->regs, SD_STS, SD_STS_ALL);

    uint64_t flags = arch_interrupts_save();
    stream->running = true;
    w32(hda->regs, REG_INTCTL, r32(hda->regs, REG_INTCTL) | INTCTL_GLOBAL | (1u << stream->index));
    w8(stream->regs, SD_CTL, SD_CTL_RUN | SD_CTL_IOCE);
    arch_interrupts_restore(flags);
    return STATUS_SUCCESS;
}

/* The hardware finished a fragment (or several, if the interrupt was late). */
static void stream_interrupt(hda_t *hda, hda_stream_t *stream, bool playback)
{
    uint32_t lpib = r32(stream->regs, SD_LPIB) % BUFFER_BYTES;
    stream->position += (lpib + BUFFER_BYTES - stream->last_lpib) % BUFFER_BYTES;
    stream->last_lpib = lpib;
    /* Interrupts come at fragment boundaries; round, in case the position register lags a little. */
    uint64_t done = (stream->position + FRAGMENT_BYTES / 2) / FRAGMENT_BYTES;

    if (playback) {
        /* Fragment `done` is playing: keep the one after it filled. */
        if (stream->handled < done + 1)
            stream->handled = done + 1; /* too late for those */
        for (; stream->handled < done + 2; stream->handled++) {
            audio_playback_pull(&hda->audio, fragment(stream, stream->handled), PERIOD_FRAMES);
            to_memory(fragment(stream, stream->handled), FRAGMENT_BYTES);
        }
    } else {
        if (done - stream->handled > FRAGMENTS)
            stream->handled = done - FRAGMENTS; /* overwritten before we came by */
        for (; stream->handled < done; stream->handled++)
            audio_capture_push(&hda->audio, fragment(stream, stream->handled), PERIOD_FRAMES);
    }
}

static void hda_interrupt(void *context)
{
    hda_t *hda = context;
    uint32_t pending = r32(hda->regs, REG_INTSTS);

    if (hda->out.regs && (pending & (1u << hda->out.index))) {
        w8(hda->out.regs, SD_STS, SD_STS_ALL);
        if (hda->out.running)
            stream_interrupt(hda, &hda->out, true);
    }
    if (hda->in.regs && (pending & (1u << hda->in.index))) {
        w8(hda->in.regs, SD_STS, SD_STS_ALL);
        if (hda->in.running)
            stream_interrupt(hda, &hda->in, false);
    }
}

static status_t hda_playback_enable(audio_device_t *audio, bool enable)
{
    hda_t *hda = audio->driver_data;
    if (!enable) {
        stream_stop(hda, &hda->out);
        return STATUS_SUCCESS;
    }
    /* The graphics side may have switched the sound of the monitor on since the codec was set up: told again. */
    if (hda->monitor)
        monitor_program(hda);
    return stream_start(hda, &hda->out, true);
}

static status_t hda_capture_enable(audio_device_t *audio, bool enable)
{
    hda_t *hda = audio->driver_data;
    if (!enable) {
        stream_stop(hda, &hda->in);
        return STATUS_SUCCESS;
    }
    return stream_start(hda, &hda->in, false);
}

static const audio_device_ops_t hda_audio_ops = {
    .playback_enable = hda_playback_enable,
    .capture_enable = hda_capture_enable,
};

/* --- Controller start -------------------------------------------------------------- */

/* The largest ring size a size register offers (capability bits 4-6: 2, 16, 256 entries). */
static uint32_t ring_size(hda_t *hda, uint32_t reg)
{
    uint8_t value = r8(hda->regs, reg);
    uint32_t choice = (value & 0x40) ? 2 : (value & 0x20) ? 1 : 0;
    w8(hda->regs, reg, (uint8_t)((value & ~3u) | choice));
    return choice == 2 ? 256 : choice == 1 ? 16 : 2;
}

static status_t controller_start(hda_t *hda)
{
    /* Controller reset; the codecs announce themselves afterwards. */
    w32(hda->regs, REG_GCTL, r32(hda->regs, REG_GCTL) & ~GCTL_RESET);
    if (!wait32(hda->regs, REG_GCTL, GCTL_RESET, 0, 100))
        return STATUS_TIMEOUT;
    thread_sleep(1000000);
    w32(hda->regs, REG_GCTL, r32(hda->regs, REG_GCTL) | GCTL_RESET);
    if (!wait32(hda->regs, REG_GCTL, GCTL_RESET, GCTL_RESET, 100))
        return STATUS_TIMEOUT;
    thread_sleep(2000000);

    hda->dma_limit = (r16(hda->regs, REG_GCAP) & 1) ? ~0ull : 0xFFFFFFFFull; /* 64-bit addresses supported? */
    status_t status = dma_alloc(hda->device, PAGE_SIZE, hda->dma_limit, &hda->rings);
    if (STATUS_IS_ERROR(status))
        return status;
    w8(hda->regs, REG_CORBCTL, 0);
    w8(hda->regs, REG_RIRBCTL, 0);
    hda->corb_size = ring_size(hda, REG_CORBSIZE);
    hda->rirb_size = ring_size(hda, REG_RIRBSIZE);

    w32(hda->regs, REG_CORBLBASE, (uint32_t)hda->rings.phys);
    w32(hda->regs, REG_CORBUBASE, (uint32_t)(hda->rings.phys >> 32));
    w16(hda->regs, REG_CORBWP, 0);
    w16(hda->regs, REG_CORBRP, 0x8000); /* reset the read pointer */
    for (int i = 0; i < 10 && !(r16(hda->regs, REG_CORBRP) & 0x8000); i++)
        thread_sleep(1000000);
    w16(hda->regs, REG_CORBRP, 0);
    for (int i = 0; i < 10 && (r16(hda->regs, REG_CORBRP) & 0x8000); i++)
        thread_sleep(1000000);

    w32(hda->regs, REG_RIRBLBASE, (uint32_t)(hda->rings.phys + PAGE_SIZE / 2));
    w32(hda->regs, REG_RIRBUBASE, (uint32_t)((hda->rings.phys + PAGE_SIZE / 2) >> 32));
    w16(hda->regs, REG_RIRBWP, 0x8000); /* reset the write pointer */
    w16(hda->regs, REG_RINTCNT, 1);
    hda->rirb_read = 0;

    w8(hda->regs, REG_CORBCTL, RING_RUN);
    /* Responses are flagged one by one but polled: the controller interrupt stays off in INTCTL. */
    w8(hda->regs, REG_RIRBCTL, RING_RUN | RIRB_INTERRUPT);
    return STATUS_SUCCESS;
}

static status_t hda_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    hda_t *hda = kcalloc(1, sizeof(*hda));
    hda_codec_t *codec = kmalloc(sizeof(*codec));
    uint32_t flags = 0;

    if (!hda || !codec) {
        kfree(hda);
        kfree(codec);
        return STATUS_OUT_OF_MEMORY;
    }
    hda->device = device;
    mutex_init(&hda->lock);

    /* The HDMI controllers of AMD graphics do not snoop (as Linux's table of controllers has it). */
    hda->no_snoop = device->id.vendor == 0x1002;
    status_t status = pci_enable_device(pci, true);
    if (!STATUS_IS_ERROR(status)) {
        hda->regs = (volatile uint8_t *)pci_map_bar(pci, 0);
        status = hda->regs ? controller_start(hda) : STATUS_DEVICE_ERROR;
    }
    if (!STATUS_IS_ERROR(status)) {
        uint16_t present = r16(hda->regs, REG_STATESTS);
        for (uint8_t address = 0; address < 15; address++) {
            if (!(present & (1u << address)))
                continue;
            memset(codec, 0, sizeof(*codec));
            codec->address = address;
            if (!read_codec(hda, codec))
                continue;
            uint32_t found = setup_codec(hda, codec, !(flags & JELLY_AUDIO_PLAYBACK), !(flags & JELLY_AUDIO_CAPTURE));
            if (!found && !(flags & JELLY_AUDIO_PLAYBACK))
                found = setup_monitor_codec(hda, codec);
            klog_info("hda: codec %u: vendor %08x, %u widgets%s%s", address, parameter(hda, codec, 0, 0),
                      codec->widget_count, (found & JELLY_AUDIO_PLAYBACK) ? ", output" : "",
                      (found & JELLY_AUDIO_CAPTURE) ? ", input" : "");
            flags |= found;
            if (!found)
                report_codec(hda, codec);
        }
        if (!flags)
            status = STATUS_NOT_FOUND; /* a controller without a usable codec */
    }
    kfree(codec);

    /* Stream descriptors: the input streams come first, then the output streams. */
    if (!STATUS_IS_ERROR(status)) {
        uint16_t gcap = r16(hda->regs, REG_GCAP);
        uint32_t inputs = (gcap >> 8) & 0xF, outputs = (gcap >> 12) & 0xF;
        if (!outputs)
            flags &= ~JELLY_AUDIO_PLAYBACK;
        if (!inputs)
            flags &= ~JELLY_AUDIO_CAPTURE;
        if (flags & JELLY_AUDIO_PLAYBACK)
            status = stream_alloc(hda, &hda->out, inputs, TAG_PLAYBACK);
        if (!STATUS_IS_ERROR(status) && (flags & JELLY_AUDIO_CAPTURE))
            status = stream_alloc(hda, &hda->in, 0, TAG_CAPTURE);
        if (!flags)
            status = STATUS_NOT_FOUND;
    }
    if (!STATUS_IS_ERROR(status))
        status = pci_enable_msi(pci, hda_interrupt, hda, &hda->irq);
    if (!STATUS_IS_ERROR(status)) {
        format(hda->audio.name, sizeof(hda->audio.name), hda->monitor ? "Monitor sound %04x:%04x" : "HD Audio %04x:%04x",
               device->id.vendor, device->id.device);
        hda->audio.flags = flags | (hda->monitor ? JELLY_AUDIO_MONITOR : 0);
        hda->audio.rate = RATE;
        hda->audio.channels = CHANNELS;
        hda->audio.period = PERIOD_FRAMES;
        hda->audio.ops = &hda_audio_ops;
        hda->audio.driver_data = hda;
        status = audio_device_register(&hda->audio);
    }
    if (STATUS_IS_ERROR(status)) {
        if (status != STATUS_NOT_FOUND)
            klog_warn("hda: %s: %s", device->name, status_name(status));
        if (hda->regs)
            w32(hda->regs, REG_INTCTL, 0);
        pci_disable_msi(pci);
        dma_free(&hda->rings);
        dma_free(&hda->out.buffer);
        dma_free(&hda->out.bdl);
        dma_free(&hda->in.buffer);
        dma_free(&hda->in.bdl);
        kfree(hda);
        return status;
    }
    device->driver_data = hda;
    return STATUS_SUCCESS;
}

static const device_match_t hda_ids[] = {
    DEVICE_MATCH_CLASS(0x04, 0x03), /* multimedia, HD Audio */
    DEVICE_MATCH_END,
};

static driver_t hda_driver = {
    .name = "intel-hda",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_AUDIO,
    .ids = hda_ids,
    .probe = hda_probe,
};

static status_t hda_module_init(void)
{
    return driver_register(&hda_driver);
}

static const char *const hda_dependencies[] = { "pci", NULL };

MODULE(.name = "intel_hda", .description = "Intel High Definition Audio", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 11, 0), .dependencies = hda_dependencies, .init = hda_module_init);
