module F (X : sig end) = struct module N = struct let a = 1 let b = 2 end end
module TT = F (struct end)
let f () = let module T = TT in T.N.a
