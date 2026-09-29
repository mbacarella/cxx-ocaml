module Fast = struct module R (D : sig end) = struct end let v = 1 end
module M = struct
  let _ = Fast.v
end
