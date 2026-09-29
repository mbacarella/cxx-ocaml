module M : sig type u end = struct
  module S = Map.Make (String)
  type u = int
  open S
  let y = empty
end
