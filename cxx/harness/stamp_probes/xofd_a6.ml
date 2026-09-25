type 'a r = {a : 'a; b : int}
module M = struct let v x = x.a let w = {a = true; b = 1} end
