// The four buttons, by their silk names: SW4 User A (GPIO39), SW5 Menu + (GPIO40), SW6 Menu - (GPIO15)
// and SW7 User B (GPIO16). A User A or User B press during the countdown steps the tab selection. Menu +
// and Menu - presses are logged only.
#pragma once

// Configures the inputs and starts the polling task. Call power_init() first: after a deep sleep wake
// GPIO15 and GPIO16 come back in RTC function, and power_init() returns them to digital function.
// A button held down when the task starts is not a press until it is released and pressed again, so the
// press that woke the badge is not reported here. The caller takes it from power_wake_cause().
void buttons_start();
