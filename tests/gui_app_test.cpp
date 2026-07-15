#include <gtest/gtest.h>

#include "gui_app.h"

TEST(GuiStyleTest, BrandTitleNotEmpty) {
    EXPECT_FALSE(gui_style::AppBrandTitle().IsEmpty());
}

TEST(GuiStyleTest, MainWindowHasExpectedSize) {
    const wxSize size = gui_style::MainWindowSize();
    EXPECT_EQ(size.GetWidth(), 800);
    EXPECT_EQ(size.GetHeight(), 1080);
}
