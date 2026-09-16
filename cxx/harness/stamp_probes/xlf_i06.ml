module M = struct
  module Fast = struct module R (D : sig end) = struct end let v = 1 end
end
let _ = M.Fast.v
