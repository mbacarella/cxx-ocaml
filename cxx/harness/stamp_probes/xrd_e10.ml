module Create(P: sig type t end) = struct
    module Priority = P
    class type ['level] prioritizer = object
        method code: 'level -> Priority.t
    end
    class virtual ['archiver] agent prioritizer limit archivers =
        let _ = (prioritizer :> 'level prioritizer) in
        let _ = (archivers :> 'archiver list) in
        object(self:'self)
            constraint 'archiver = 'level list
            val mutable archivers_ = archivers
            val mutable limit_ = prioritizer#code limit
            method virtual private event: 'level -> string -> 'archiver
            method setlimit limit = limit_ <- prioritizer#code limit
            method enabled limit = prioritizer#code limit >= limit_
        end
end
