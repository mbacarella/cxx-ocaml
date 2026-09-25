class a = object method top = "t" end
module M = struct class a = object method m = 1 end end
class b = object inherit M.a end
