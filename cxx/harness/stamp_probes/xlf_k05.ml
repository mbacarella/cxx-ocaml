module M : sig type t val zero : t end = struct
  type t = int
  module type S = sig val zero : t val one : t val add : t -> t -> t end
  let f (x : (module S)) = x
  let zero = 0
end
