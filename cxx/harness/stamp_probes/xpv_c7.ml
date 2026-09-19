type z = int
module type S = sig type t val to_string : t -> string val x : t end
let forget x = let module M = (val x : S) in (module M : S)
