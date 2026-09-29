type z = int
module type S = sig type t val to_string : t -> string val x : t end
let forget x = let module M = (val x : S) in
  let module N = struct include M let x = M.x end in ()
