module M : sig type t val zero : t end = struct
  type t = int
  module type S = sig val zero : t val one : t val add : t -> t -> t end
  let impl : (module S) = (module Int : S)
  let zero = 0
end
