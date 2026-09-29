module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let y = match S.empty with _ -> 1
end
