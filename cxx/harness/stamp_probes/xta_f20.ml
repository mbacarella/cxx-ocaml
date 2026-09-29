module TT = struct module H = Ephemeron.K2.Make(Int)(Int) end
let f () = let module T = TT in T.H.create
