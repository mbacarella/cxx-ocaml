module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  open S
  let g (x : t) = 1
end
