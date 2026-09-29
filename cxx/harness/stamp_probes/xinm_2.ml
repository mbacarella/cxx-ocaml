module M = struct module N = struct class ['x] a (x : 'x) = object method m = x end end end
class b = object inherit [int] M.N.a 3 end
