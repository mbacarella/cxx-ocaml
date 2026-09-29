module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let _ = S.(empty)
end
