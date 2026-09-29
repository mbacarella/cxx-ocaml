module Fast = struct let v = 2 end
module M = struct
  module Fast = struct module R (D : sig end) = struct end let v = 1 end
  let _ = Fast.v
end
