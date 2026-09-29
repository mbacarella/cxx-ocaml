module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  open List open S
  let _ = length [1]
end
