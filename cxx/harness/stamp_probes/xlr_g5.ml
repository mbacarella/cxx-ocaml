let f () = let module TT = struct module N = struct let x = 1 let y = 2 end let
  z = 3 end in let module T = TT in T.z
