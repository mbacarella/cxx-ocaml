module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
    end
end
module Create(P: Order.Total) = struct
    module Priority = P

    class type ['level] prioritizer = object
        method code: 'level -> Priority.t
        method tag: 'level -> string
    end

    class ['level] event prioritizer level message =
        let prioritizer = (prioritizer :> 'level prioritizer) in
        object
            method prioritizer = prioritizer
            method level: 'level = level
            method message: string = message
        end

    class type ['event] archiver = object
        constraint 'event = 'level #event
        method emit: 'event -> unit
    end

    class virtual ['archiver] agent prioritizer limit archivers =
        let _ = (prioritizer :> 'level prioritizer) in
        let _ = (archivers :> 'archiver list) in
        object(self:'self)
            constraint 'event = 'level #event
            constraint 'archiver = 'event #archiver

            val mutable archivers_ = archivers
            val mutable limit_ = prioritizer#code limit

            method virtual private event: 'level -> string -> 'event

            method setlimit limit = limit_ <- prioritizer#code limit
            method enabled limit = prioritizer#code limit >= limit_

            method private put:
                type a b. 'level -> ('event -> b) ->
                (a, unit, string, string, string, b) format6 -> a
                = fun level cont ->
                    let f message =
                        let e = self#event level message in
                        if self#enabled level then
                            List.iter (fun j -> j#emit e) archivers_;
                        cont e
                    in
                    Printf.ksprintf f
        end
end
module Arg = struct type t = int  let compare a b = b - a end
module Basic = struct include Create(Arg) end
