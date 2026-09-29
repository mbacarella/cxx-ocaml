module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let _ = let open S in empty
end
