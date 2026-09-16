module M : sig end = struct
  module Fast = struct module R (D : sig type data end) = struct end end
end
