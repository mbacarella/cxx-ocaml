module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  open S
  let y = (empty : t)
end
