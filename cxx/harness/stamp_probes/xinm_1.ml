module M = struct class a = object method m = 1 val v = 2 end end
class b = object inherit M.a method n = 3 end
