module M : sig type u end = struct
  module S = Set.Make (String)
  type u = int
  let y = string_of_int @@ S.cardinal @@ S.empty
end
