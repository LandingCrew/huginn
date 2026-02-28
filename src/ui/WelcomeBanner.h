#pragma once

namespace Huginn::UI
{
    class WelcomeBanner
    {
    public:
        static WelcomeBanner& GetSingleton();

        void Show();
        void Draw();

        bool IsVisible() const { return m_lingerTime > 0.0f; }

    private:
        WelcomeBanner() = default;
        ~WelcomeBanner() = default;
        WelcomeBanner(const WelcomeBanner&) = delete;
        WelcomeBanner& operator=(const WelcomeBanner&) = delete;

        // Display duration in seconds
        static constexpr float DISPLAY_TIME = 7.0f;
        static constexpr float FADE_TIME = 1.0f;

        float m_lingerTime = 0.0f;
        bool m_firstFrame = false;
    };
}
