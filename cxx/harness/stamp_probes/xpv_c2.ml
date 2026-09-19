type z = int
module type S = sig type t val to_string : t -> string val x : t end
let forget (type s) x =
  let module M = (val x : S with type t = s) in ()
