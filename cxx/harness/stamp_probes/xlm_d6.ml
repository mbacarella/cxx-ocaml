module P = struct module M = Map.Make(String) end
let f () =
   let module N = Map.Make(String) in
   N.add "sum" 41 N.empty
