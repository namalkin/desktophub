#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct DialogField {
    std::wstring label;
    std::wstring hint;   // подсказка под полем (можно пустую)
    std::wstring value;  // на входе — начальное значение, на выходе — введённое
    bool required = false;
    bool multiline = false;  // высокое поле с переносом строк: мысли, благодарность
};

struct DialogSpec {
    std::wstring title;
    std::wstring okLabel = L"Сохранить";
    std::vector<DialogField> fields;
};

// Модальный ввод. Возвращает false, если пользователь отменил.
//
// owner — окно верхнего уровня, которое владеет диалогом (скрытое окно приложения);
// anchor — окно, по которому диалог центрируется и у которого берётся DPI, обычно блок.
// Разделены потому, что блок может быть дочерним окном рабочего стола: сделать такой
// блок владельцем нельзя — владельцем стал бы сам рабочий стол.
bool ShowDialog(HWND owner, HWND anchor, DialogSpec& spec);
