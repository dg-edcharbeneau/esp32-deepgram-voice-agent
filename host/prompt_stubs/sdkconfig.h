/*
 * Host stub for sdkconfig.h. agent_prompt.c still includes it, but the prompt
 * no longer has build-gated blocks -- the speech-stack choice, the mic-gate
 * toggle and the one-line prompt override were all removed -- so there is
 * nothing left to define. Kept as the include target, and as the place any
 * future gate's host default would go.
 */
#pragma once
