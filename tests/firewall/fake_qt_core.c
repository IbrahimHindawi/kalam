#if defined(_WIN32)
#define KALAM_FAKE_QT_EXPORT __declspec(dllexport)
#else
#define KALAM_FAKE_QT_EXPORT
#endif

KALAM_FAKE_QT_EXPORT int kalam_fake_qt_symbol(void)
{
    return 0;
}
