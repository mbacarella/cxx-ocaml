let f b =
   let module N = Map.Make(String) in
   if b then N.empty else N.empty
