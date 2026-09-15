module M : sig val y : Set.Make(String).t end = struct
  module S = Set.Make (String)
  let y = S.empty
end
