module N : sig end = struct
  module S = Set.Make (String)
  let y = S.empty
end
