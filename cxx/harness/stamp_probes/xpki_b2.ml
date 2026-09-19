type z = int
module type S = sig type t val x : t end
let forget x = let module M = (val x : S with type t = int) in (module M : S)
