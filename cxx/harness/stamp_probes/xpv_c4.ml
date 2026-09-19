type z = int
module type S = sig type t val to_string : t -> string val x : t end
let forget (type s) (x : (module S with type t = s)) = (module (val x) : S)
