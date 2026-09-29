module TT = struct module N : sig val x : int end = struct let x = 1 let y = 2
  end end
let f () = let module T = TT in T.N.x
