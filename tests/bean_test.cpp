#include <gtest/gtest.h>

#include "bean/bean.h"

TEST(BeanStyleTest, GameTitleNotEmpty) {
    EXPECT_FALSE(bean_style::GameTitle().IsEmpty());
}

TEST(BeanStyleTest, GameTitleIsExpected) {
    EXPECT_EQ(bean_style::GameTitle(), wxT("Bean Game"));
}

TEST(BeanStyleTest, WindowSizeIsPositive) {
    const wxSize size = bean_style::WindowSize();
    EXPECT_GT(size.GetWidth(), 0);
    EXPECT_GT(size.GetHeight(), 0);
}

TEST(BeanStyleTest, WindowSizeIsExpected) {
    const wxSize size = bean_style::WindowSize();
    EXPECT_EQ(size.GetWidth(), 760);
    EXPECT_EQ(size.GetHeight(), 820);
}

TEST(BeanStyleTest, MinWindowSizeIsPositive) {
    const wxSize size = bean_style::MinWindowSize();
    EXPECT_GT(size.GetWidth(), 0);
    EXPECT_GT(size.GetHeight(), 0);
}

TEST(BeanStyleTest, MinWindowSizeIsExpected) {
    const wxSize size = bean_style::MinWindowSize();
    EXPECT_EQ(size.GetWidth(), 620);
    EXPECT_EQ(size.GetHeight(), 700);
}

TEST(BeanStyleTest, MinWindowSizeSmallerThanWindowSize) {
    const wxSize win = bean_style::WindowSize();
    const wxSize min_win = bean_style::MinWindowSize();
    EXPECT_LE(min_win.GetWidth(), win.GetWidth());
    EXPECT_LE(min_win.GetHeight(), win.GetHeight());
}

TEST(BeanStyleTest, GamePanelSizeIsPositive) {
    const wxSize size = bean_style::GamePanelSize();
    EXPECT_GT(size.GetWidth(), 0);
    EXPECT_GT(size.GetHeight(), 0);
}

TEST(BeanStyleTest, GamePanelSizeIsExpected) {
    const wxSize size = bean_style::GamePanelSize();
    EXPECT_EQ(size.GetWidth(), 520);
    EXPECT_EQ(size.GetHeight(), 520);
}

TEST(BeanStyleTest, GamePanelSizeFitsInWindow) {
    const wxSize win = bean_style::WindowSize();
    const wxSize panel = bean_style::GamePanelSize();
    EXPECT_LE(panel.GetWidth(), win.GetWidth());
    EXPECT_LE(panel.GetHeight(), win.GetHeight());
}

TEST(BeanStyleTest, TimerIntervalMsIsPositive) {
    EXPECT_GT(bean_style::TimerIntervalMs(), 0);
}

TEST(BeanStyleTest, TimerIntervalMsIsExpected) {
    EXPECT_EQ(bean_style::TimerIntervalMs(), 95);
}
