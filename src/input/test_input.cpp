/**
 * Input Module Test
 */
#include "input.hpp"
#include <cstdio>
#include <cstring>
#include <windows.h>

int main(int argc, char* argv[]) {
    printf("=== Input Module Test ===\n\n");

    auto backend = create_input_backend();
    printf("Active backend: %s\n\n", backend->name());

    // Test mouse move + click simulation coordinates
    Rect game_win = {100, 200, 300, 300};
    printf("Simulated coordinates (window %d,%d %dx%d):\n", game_win.x, game_win.y, game_win.w, game_win.h);
    printf("  Center click: MouseMove(%d,%d) + Click\n\n",
           game_win.x + game_win.w/2, game_win.y + game_win.h/2);

    // VK name lookup
    printf("VK name lookup:\n");
    uint16_t keys[] = {VK_SPACE, VK_RETURN, VK_ESCAPE, 'W', 'A'};
    for (int i = 0; i < 5; i++)
        printf("  VK 0x%04X = '%s'\n", keys[i], vk_name(keys[i]));

    if (argc > 1 && strcmp(argv[1], "--execute") == 0) {
        printf("\n*** Moving mouse to (400,300) and clicking in 3s ***\n");
        printf("Move cursor away to cancel!\n");
        Sleep(3000);
        backend->move_mouse(400, 300);
        Sleep(100);
        backend->click(MouseButton::Left);
        printf("Done.\n");
    }

    backend->shutdown();
    printf("\nTest complete.\n");
    return 0;
}
