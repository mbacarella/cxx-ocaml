module type S = sig type t val apply : t -> t val x : t end
let int = (module struct type t = int let apply x = x let x = 1 end : S)
let print x = let module M = (val x : S) in ignore M.x
let apply x = let module M = (val x : S) in
  let module N = struct include M let x = apply x end in (module N : S)
let () = List.iter print (List.filter (fun _ -> true) [int])
