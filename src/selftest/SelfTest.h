#pragma once
// SelfTest.h — headless data-layer verification against the real local
// agent data (exit code 0/1, JSON summary at the end).

class SelfTest {
public:
    static int run();
};
