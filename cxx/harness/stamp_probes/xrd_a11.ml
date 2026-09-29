module M = struct class a = object method m = 1 method n = 2 end end
class b = object inherit M.a end
