#include <gpiod.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <cstdlib>

int main() {
    std::cout << "\n=======================================================\n";
    std::cout << "  АППАРАТНЫЙ ТЕСТ СВЕТОДИОДОВ ORANGE PI PC (libgpiod) \n";
    std::cout << "=======================================================\n\n";

    // Check if looper is running
    int ps_ret = std::system("pgrep -x looper >/dev/null 2>&1");
    if (ps_ret == 0) {
        std::cerr << "ВНИМАНИЕ: Процесс 'looper' сейчас запущен и занимает пины!\n";
        std::cerr << "Останови его (нажми Ctrl+C в его терминале или закрой),\n";
        std::cerr << "а затем запусти этот тест снова: ./test_led\n\n";
        return 1;
    }

    struct gpiod_chip* chip = gpiod_chip_open_by_name("gpiochip0");
    if (!chip) {
        std::cerr << "ОШИБКА: Не удалось открыть /dev/gpiochip0!\n";
        return 1;
    }

    // 1st LED (Status): Pin 7 (PA6), Pin 11 (PA1), Pin 13 (PA0)
    struct gpiod_line* led1_r = gpiod_chip_get_line(chip, 6);
    struct gpiod_line* led1_g = gpiod_chip_get_line(chip, 1);
    struct gpiod_line* led1_b = gpiod_chip_get_line(chip, 0);

    // 2nd LED (Clip / Signal): Pin 8 (PA13), Pin 10 (PA14)
    struct gpiod_line* led2_r = gpiod_chip_get_line(chip, 13);
    struct gpiod_line* led2_g = gpiod_chip_get_line(chip, 14);

    if (!led2_r || !led2_g) {
        std::cerr << "ОШИБКА: Линии PA13 или PA14 не найдены на gpiochip0!\n";
        gpiod_chip_close(chip);
        return 1;
    }

    bool led1_ok = (led1_r && led1_g && led1_b &&
                    gpiod_line_request_output(led1_r, "test_led1_r", 0) == 0 &&
                    gpiod_line_request_output(led1_g, "test_led1_g", 0) == 0 &&
                    gpiod_line_request_output(led1_b, "test_led1_b", 0) == 0);

    int ret2_r = gpiod_line_request_output(led2_r, "test_led2_r", 0);
    int ret2_g = gpiod_line_request_output(led2_g, "test_led2_g", 0);

    if (ret2_r < 0 || ret2_g < 0) {
        std::cerr << "ОШИБКА: Не удалось захватить пины PA13/PA14 (код " << ret2_r << "/" << ret2_g << ")\n";
        if (led1_ok) {
            gpiod_line_release(led1_r);
            gpiod_line_release(led1_g);
            gpiod_line_release(led1_b);
        }
        gpiod_chip_close(chip);
        return 1;
    }

    if (led1_ok) {
        std::cout << "[ЭТАП 1] Проверка первого (статусного) светодиода:\n";
        std::cout << "  -> Зажигаем Красный на 1-м диоде...\n";
        gpiod_line_set_value(led1_r, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        gpiod_line_set_value(led1_r, 0);

        std::cout << "  -> Зажигаем Зеленый на 1-м диоде...\n";
        gpiod_line_set_value(led1_g, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        gpiod_line_set_value(led1_g, 0);

        std::cout << "  -> Зажигаем Синий на 1-м диоде...\n";
        gpiod_line_set_value(led1_b, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        gpiod_line_set_value(led1_b, 0);
        std::cout << "  1-й светодиод проверен успешно.\n\n";
    }

    std::cout << "[ЭТАП 2] Проверка ВТОРОГО светодиода перегрузки/сигнала:\n";
    std::cout << "  -> [ТЕСТ 2.1] Подаем +3.3V на Pin 8 (PA13 / Красный) на 3 секунды...\n";
    gpiod_line_set_value(led2_r, 1);
    gpiod_line_set_value(led2_g, 0);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::cout << "  -> [ТЕСТ 2.2] Подаем +3.3V на Pin 10 (PA14 / Зеленый) на 3 секунды...\n";
    gpiod_line_set_value(led2_r, 0);
    gpiod_line_set_value(led2_g, 1);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::cout << "  -> [ТЕСТ 2.3] Подаем +3.3V на ОБА (Pin 8 + Pin 10 / Желтый) на 3 секунды...\n";
    gpiod_line_set_value(led2_r, 1);
    gpiod_line_set_value(led2_g, 1);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::cout << "  -> [ТЕСТ 2.4] Быстрое мигание (10 тактов)...\n";
    for (int i = 0; i < 10; ++i) {
        gpiod_line_set_value(led2_r, (i % 2 == 0) ? 1 : 0);
        gpiod_line_set_value(led2_g, (i % 2 == 1) ? 1 : 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    // Turn off everything
    gpiod_line_set_value(led2_r, 0);
    gpiod_line_set_value(led2_g, 0);

    gpiod_line_release(led2_r);
    gpiod_line_release(led2_g);

    if (led1_ok) {
        gpiod_line_release(led1_r);
        gpiod_line_release(led1_g);
        gpiod_line_release(led1_b);
    }
    gpiod_chip_close(chip);

    std::cout << "\n=======================================================\n";
    std::cout << "  Тест завершен! Все линии сброшены в 0.\n";
    std::cout << "=======================================================\n\n";
    return 0;
}
