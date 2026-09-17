module M = struct class a = object method m = 1 end end
let f () = object inherit M.a end
