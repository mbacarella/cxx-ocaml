let f () =
   let module N = Hashtbl.Make(String) in
   N.create 1
