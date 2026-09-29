let f () =
   let module N = Set.Make(String) in
   let x = N.empty in
   let y = N.empty in
   N.equal x y
