module TT = struct module N = struct type t = C | D let x = 1 end end
let f () = let module T = TT in T.N.C
