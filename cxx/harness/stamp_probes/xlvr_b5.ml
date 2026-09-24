module A = struct
  module M = Map.Make (String)
  module B = struct type 'a u = 'a M.t end
end
