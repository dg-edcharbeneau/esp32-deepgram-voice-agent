/*
 * Host stub for the build-gated blocks. Defaults match sdkconfig.defaults, and
 * run.sh overrides them with -D to dump the other builds.
 */
#pragma once
#ifndef CONFIG_DEEPGRAM_AGENT_PROMPT
#define CONFIG_DEEPGRAM_AGENT_PROMPT ""
#endif
#ifndef CONFIG_SPEECH_STACK_FLUX
#define CONFIG_SPEECH_STACK_FLUX 1
#endif
#ifndef CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS
#define CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS 1
#endif
