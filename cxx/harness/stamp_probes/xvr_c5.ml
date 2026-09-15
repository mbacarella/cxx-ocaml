module M : sig module S : Set.S end = struct
  module S = Set.Make (String)
  let _ = S.empty
end
