#if defined(_WIN32)
#define KALAM_RELOAD_TEST_EXPORT __declspec(dllexport)
#else
#define KALAM_RELOAD_TEST_EXPORT
#endif

extern "C" KALAM_RELOAD_TEST_EXPORT int
reload_fake_not_the_game_entry(void)
{
    return 1;
}
