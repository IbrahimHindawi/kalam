#if defined(_WIN32)
#define KALAM_FAKE_QT_IMPORT __declspec(dllimport)
#else
#define KALAM_FAKE_QT_IMPORT
#endif

KALAM_FAKE_QT_IMPORT int kalam_fake_qt_symbol(void);

int main(void)
{
    return kalam_fake_qt_symbol();
}
