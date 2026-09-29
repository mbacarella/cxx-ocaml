module M = struct class a (x:int) = object method m = x end end
class b x = object inherit M.a x end
