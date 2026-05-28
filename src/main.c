#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "system.h"
#include <SDL3/SDL.h>
#include "nones.h"
#include "utils.h"

#define VERSION "v0.4.0"

static void About(void)
{
    printf("nones " VERSION " by Matt W\n");
}

static void Usage(void)
{
    About();
    printf("Usage: nones \"game.nes\" [options...]\n");
}

static void Help(void)
{
    Usage();
    printf("Options:\n"
           "  --help                             Display this information\n"
           "  --version                          Display version information\n"
           "  --fullscreen                       Start in fullscreen\n"
           "  --aspect-ratio=\"aspect-ratio-mode\" Set the screen aspect ratio: 0 = 1:1 (default), 1 = 8:7\n"
           "  --sdl-audio-driver=\"driver-name\"   Set the preferred audio driver for SDL to use\n"
           "  --ppu-warmup                       Enable the ppu warm up delay found on the NES-001(Will break some famicom games)\n"
           "  --apu-swap-duty-cycles             Enable the use of swapped duty cycles for the square/pulse channels(Needed for older famiclone games)\n"
           "  --sample-rate=\"sample-rate-mode\"   Set the audio device sample-rate: 0 = 44100Hz (default), 1 = 48000Hz, 2 = 96000Hz, 3 = 192000Hz\n");
}

static const int sample_rates[] = 
{
    44100,
    48000,
    96000,
    192000
};

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("No file was provided\n");
        Usage();
        return EXIT_FAILURE;
    }

    int sample_rate_mode = 0;
    int aspect_ratio = 0;
    bool fullscreen = false;
    bool ppu_warmup = false;
    bool swap_duty_cycles = false;
    bool override_audio_driver = false;
    char audio_driver[128] = {"\0"};

    for (int i = 1; i < argc; i++)
    {
        if (i > 1 && strlen(argv[i]) > 127)
            continue;

        if (!strcmp((argv[i]), "--help"))
        {
            Help();
            return EXIT_SUCCESS;
        }

        if (!strcmp((argv[i]), "--version"))
        {
            About();
            return EXIT_SUCCESS;
        }

        if (strstr((argv[i]), "--aspect-ratio="))
        {
            char *delim_pos = strchr(argv[i], '=');
            if (delim_pos != NULL)
            {
                char *aspect_ratio_str = delim_pos + 1;
                char *end;
                int new_aspect_ratio = (int)strtol(aspect_ratio_str, &end, 10);
                if (new_aspect_ratio >= 0 && new_aspect_ratio <= 1)
                    aspect_ratio = new_aspect_ratio;
                else
                {
                    printf("Invalid sample rate mode!\n");
                    Usage();
                    return EXIT_FAILURE;
                }
            }
        }

        if (!strcmp((argv[i]), "--fullscreen"))
            fullscreen = true;

        if (!strcmp((argv[i]), "--ppu-warmup"))
            ppu_warmup = true;

        if (!strcmp((argv[i]), "--apu-swap-duty-cycles"))
            swap_duty_cycles = true;

        if (strstr((argv[i]), "--sample-rate="))
        {
            char *delim_pos = strchr(argv[i], '=');
            if (delim_pos != NULL)
            {
                char *sample_mode_str = delim_pos + 1;
                char *end;
                int new_sample_mode = (int)strtol(sample_mode_str, &end, 10);
                if (new_sample_mode >= 0 && new_sample_mode <= (int)ARRAY_SIZE(sample_rates))
                    sample_rate_mode = new_sample_mode;
                else
                {
                    printf("Invalid sample rate mode!\n");
                    Usage();
                    return EXIT_FAILURE;
                }
            }
        }

        if (strstr((argv[i]), "--sdl-audio-driver="))
        {
            char *delim_pos = strchr(argv[i], '=');
            if (delim_pos != NULL)
            {
                override_audio_driver = true;
                char *driver_name = delim_pos + 1;
                snprintf(audio_driver, sizeof(audio_driver), "%s", driver_name);
            }
        }
    }

    const int sample_rate = sample_rates[sample_rate_mode];

    Nones nones;
    NonesRun(&nones, ppu_warmup, fullscreen, aspect_ratio, swap_duty_cycles, sample_rate,
            argv[1], override_audio_driver ? audio_driver : NULL);
    return EXIT_SUCCESS;
}
