type z = int
module type S3 = sig val x : bool end
let f = function Some (module M : S3) when M.x -> 1 | Some _ -> 2 | None -> 3
let () = ignore (f None)
