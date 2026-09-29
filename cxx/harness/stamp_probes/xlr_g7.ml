let f () = let module TT = struct module N = struct let x = 1 let y = 2 module Q
  = struct let q = 1 end end end in let module T = TT in T.N.x
