let f () = let module TT = struct module N = struct let x = 1 let y = 2 end end
  in let module T = TT in let module U = TT in T.N.x + U.N.x
