module type S = sig module N : sig val x : int val y : int end end
module TT : S = struct module N = struct let x = 1 let y = 2 end end
let f () = let module T = TT in T.N.x
